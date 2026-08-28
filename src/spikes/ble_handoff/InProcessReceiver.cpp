#if defined(CROSSINK_BLE_HANDOFF_RECEIVER) || defined(CROSSINK_IN_PROCESS_RECEIVER)

#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <freertos/FreeRTOS.h>

#include <array>
#include <cstring>

#include "BleHandoffNvs.h"
#include "DashboardSlotSelection.h"
#include "DashboardTransfer.h"
#include "InProcessReceiver.h"
#include "ReceiverWindow.h"

namespace {

constexpr char DEVICE_NAME[] = "XTEINK-X3-RX";
constexpr char SERVICE_UUID[] = "8c9f9d10-7c6d-4c8e-a2cb-49586da45d10";
constexpr char WRITE_UUID[] = "8c9f9d11-7c6d-4c8e-a2cb-49586da45d10";
constexpr char STATUS_UUID[] = "8c9f9d12-7c6d-4c8e-a2cb-49586da45d10";
constexpr size_t MAX_FRAME_SIZE = dashboard::MAX_PACKAGE_SIZE + 7;

portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
std::array<uint8_t, MAX_FRAME_SIZE> pendingFrame{};
size_t pendingLength = 0;
volatile bool framePending = false;
BLECharacteristic* statusCharacteristic = nullptr;
dashboard::TransferAssembler assembler;
dashboard::ReceiverWindow receiverWindow(0);
bool packageAccepted = false;
uint32_t acceptedPackageId = 0;
uint16_t acceptedByteCount = 0;

void writeU32(uint8_t* out, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}

// `detail` names the underlying reason behind a failure code, so the phone can
// explain a rejection without anyone attaching a serial cable to the X3: the
// dashboard::Status behind 0x12, the PersistStatus behind 0x13, and the
// TransferStatus behind 0x11. Zero for the success codes, which carry no
// further reason. Older phone builds read only the first seven bytes and are
// unaffected by the extra one.
void notify(uint8_t code, uint32_t packageId, uint16_t received, uint8_t detail = 0) {
  if (statusCharacteristic == nullptr) return;
  uint8_t value[8] = {code};
  writeU32(value + 1, packageId);
  value[5] = static_cast<uint8_t>(received);
  value[6] = static_cast<uint8_t>(received >> 8U);
  value[7] = detail;
  statusCharacteristic->setValue(value, sizeof(value));
  statusCharacteristic->notify();
}

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    Serial.println("BLE-RX connected");
    notify(0x01, 0, 0);
  }
  void onDisconnect(BLEServer*) override {
    Serial.println("BLE-RX disconnected");
    BLEDevice::startAdvertising();
  }
};

class WriteCallbacks final : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    if (length == 0 || length > pendingFrame.size() || data == nullptr) return;
    bool dropped = false;
    portENTER_CRITICAL(&pendingMux);
    if (!framePending) {
      std::memcpy(pendingFrame.data(), data, length);
      pendingLength = length;
      framePending = true;
    } else {
      dropped = true;
    }
    portEXIT_CRITICAL(&pendingMux);
    if (dropped) Serial.println("BLE-RX frame dropped: pending buffer occupied");
  }
};

ServerCallbacks serverCallbacks;
WriteCallbacks writeCallbacks;

}  // namespace

namespace dashboard {

ReceiverResult runReceiverWindow(const uint32_t windowMs) {
  // Start the clock before the radio comes up, so the time the stack takes to
  // initialise counts against the window rather than extending it.
  receiverWindow = ReceiverWindow(millis(), windowMs);
  if (!BLEDevice::init(DEVICE_NAME)) return ReceiverResult::TimedOut;
  BLEServer* server = BLEDevice::createServer();
  if (server == nullptr) return ReceiverResult::TimedOut;
  server->setCallbacks(&serverCallbacks);
  BLEService* service = server->createService(SERVICE_UUID);
  BLECharacteristic* writable = service->createCharacteristic(WRITE_UUID, BLECharacteristic::PROPERTY_WRITE);
  statusCharacteristic =
      service->createCharacteristic(STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  writable->setCallbacks(&writeCallbacks);
  service->start();
  BLEAdvertising* advertising = server->getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(false);
  advertising->start();
  Serial.printf("BLE-RX ready free=%u maxAlloc=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  while (true) {
    const ReceiverWindowAction windowAction = receiverWindow.actionAt(millis(), packageAccepted);
    if (windowAction == ReceiverWindowAction::ReturnAccepted) return ReceiverResult::Accepted;
    if (windowAction == ReceiverWindowAction::ReturnTimedOut) {
      Serial.println("BLE-RX window timed out; returning to reader");
      return ReceiverResult::TimedOut;
    }

    // Static, not a local: MAX_FRAME_SIZE tracks MAX_PACKAGE_SIZE, and a
    // kilobyte-plus array would be a large slice of this task's stack. Only
    // runReceiverWindow() touches it, and it is not reentrant.
    static std::array<uint8_t, MAX_FRAME_SIZE> frame{};
    size_t length = 0;
    portENTER_CRITICAL(&pendingMux);
    if (framePending) {
      length = pendingLength;
      std::memcpy(frame.data(), pendingFrame.data(), length);
      framePending = false;
    }
    portEXIT_CRITICAL(&pendingMux);
    if (length == 0) {
      delay(5);
      continue;
    }

    const TransferResult result = assembler.accept(frame.data(), length);
    Serial.printf("BLE-RX frame type=%u length=%u status=%u package=%u received=%u\n", frame[0], length,
                  static_cast<unsigned>(result.status), result.packageId, result.received);
    if (result.status == TransferStatus::Ready) {
      notify(0x01, result.packageId, result.received);
      continue;
    }
    if (result.status == TransferStatus::Progress) {
      notify(0x02, result.packageId, result.received);
      continue;
    }
    if (result.status != TransferStatus::Complete) {
      notify(0x11, result.packageId, result.received, static_cast<uint8_t>(result.status));
      continue;
    }

    static PersistedPackage persisted;
    Status persistDetail = Status::Ok;
    const PersistStatus persistedStatus =
        persistIfNewer(assembler.bytes().data(), assembler.length(), persisted, &persistDetail);
    Serial.printf("BLE-RX persistIfNewer status=%u detail=%u\n", static_cast<unsigned>(persistedStatus),
                  static_cast<unsigned>(persistDetail));
    if (persistedStatus == PersistStatus::Stale) {
      notify(0x10, receiverStatusPackageId(true, result.packageId, persisted.header.packageId), result.received);
      continue;
    }
    if (persistedStatus == PersistStatus::InvalidPackage) {
      notify(0x12, result.packageId, result.received, static_cast<uint8_t>(persistDetail));
      continue;
    }
    if (persistedStatus != PersistStatus::Ok) {
      notify(0x13, result.packageId, result.received, static_cast<uint8_t>(persistedStatus));
      continue;
    }
    packageAccepted = true;
    acceptedPackageId = result.packageId;
    acceptedByteCount = result.received;
    Serial.printf("PERSISTED package=%u length=%u crc=%08x\n", persisted.header.packageId, persisted.length,
                  persisted.header.crc);
  }
}

void notifyReceiverStatus(const uint8_t code) { notify(code, acceptedPackageId, acceptedByteCount); }

void teardownReceiver() {
  static bool tornDown = false;
  if (tornDown) return;
  tornDown = true;
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) advertising->stop();
  BLEDevice::deinit(true);
}

}  // namespace dashboard

#endif
