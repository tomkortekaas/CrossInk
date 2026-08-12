#include "DashboardBleTransport.h"

#if defined(CROSSINK_ENABLE_DASHBOARD_BLE_PROBE) && CROSSINK_ENABLE_DASHBOARD_BLE_PROBE

#include <cstring>
#include <esp_random.h>

#include "Logging.h"

namespace probe {

bool DashboardBleTransport::begin(DashboardBleProbe& probe, const PairingMode pairingMode,
                                  PasskeyDisplay* const passkeyDisplay) {
  probe_ = &probe;
  pairingMode_ = pairingMode;
  passkeyDisplay_ = passkeyDisplay;
  passkey_ = pairingMode == PairingMode::Onboarding ? 100000U + (esp_random() % 900000U) : 0;
  connectionCount_.store(0);
  hasConnected_.store(false);
  if (!NimBLEDevice::init(kAdvertisingName)) {
    LOG_ERR("BLE", "Dashboard probe: NimBLE init failed");
    return false;
  }

  initialized_ = true;
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(pairingMode == PairingMode::Onboarding ? BLE_HS_IO_DISPLAY_ONLY
                                                                        : BLE_HS_IO_NO_INPUT_OUTPUT);
  // NimBLE-Arduino 2.3.8 invokes onPassKeyDisplay() only while this static
  // value remains its default placeholder. The callback supplies the actual
  // per-onboarding random passkey, or rejects pairing in the timer route.
  NimBLEDevice::setSecurityPasskey(123456);
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

  NimBLECharacteristic* writeCharacteristic =
      service->createCharacteristic(kWriteUuid,
                                    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                                        NIMBLE_PROPERTY::WRITE_AUTHEN,
                                    kMaxFrameBytes);
  statusCharacteristic_ = service->createCharacteristic(
      kStatusUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN,
      sizeof(statusValue_));
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

bool DashboardBleTransport::end() {
  if (!initialized_) {
    return true;
  }
  NimBLEAdvertising* const advertising = NimBLEDevice::getAdvertising();
  if (advertising != nullptr && advertising->isAdvertising()) {
    advertising->stop();
  }
  if (server_ != nullptr) {
    for (const uint16_t handle : server_->getPeerDevices()) {
      server_->disconnect(handle);
    }
  }
  delay(50);
  const bool result = NimBLEDevice::deinit(true);
  initialized_ = false;
  probe_ = nullptr;
  server_ = nullptr;
  statusCharacteristic_ = nullptr;
  passkeyDisplay_ = nullptr;
  passkey_ = 0;
  return result;
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

void DashboardBleTransport::onConnect(NimBLEServer*, NimBLEConnInfo&) {
  hasConnected_.store(true);
  connectionCount_.fetch_add(1);
}

void DashboardBleTransport::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {
  if (initialized_) {
    NimBLEDevice::startAdvertising();
  }
}

uint32_t DashboardBleTransport::onPassKeyDisplay() {
  if (pairingMode_ != PairingMode::Onboarding) {
    if (server_ != nullptr) {
      for (const uint16_t handle : server_->getPeerDevices()) {
        server_->disconnect(handle);
      }
    }
    LOG_ERR("BLE", "Dashboard probe: pairing request rejected outside onboarding");
    return 0;
  }
  if (passkeyDisplay_ != nullptr) {
    passkeyDisplay_->showPasskey(passkey_);
  }
  return passkey_;
}

void DashboardBleTransport::onAuthenticationComplete(NimBLEConnInfo& connection) {
  if (!connection.isEncrypted() || !connection.isAuthenticated()) {
    if (server_ != nullptr) {
      server_->disconnect(connection.getConnHandle());
    }
    LOG_ERR("BLE", "Dashboard probe: unauthenticated connection rejected");
  }
}

}  // namespace probe
#endif
