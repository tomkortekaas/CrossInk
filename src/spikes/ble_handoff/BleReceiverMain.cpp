#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <cstring>
#include <freertos/FreeRTOS.h>

#include "BleHandoffNvs.h"

namespace {

constexpr char DEVICE_NAME[] = "XTEINK-X3-RX";
constexpr char SERVICE_UUID[] = "8c9f9d10-7c6d-4c8e-a2cb-49586da45d10";
constexpr char WRITE_UUID[] = "8c9f9d11-7c6d-4c8e-a2cb-49586da45d10";

portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
std::array<uint8_t, ble_handoff::MAX_PAYLOAD_SIZE> pendingPayload{};
size_t pendingLength = 0;
volatile bool payloadPending = false;
size_t rejectedLength = 0;
volatile bool rejectionPending = false;

const char* statusName(const ble_handoff::Status status) {
  switch (status) {
    case ble_handoff::Status::Ok:
      return "ok";
    case ble_handoff::Status::InvalidArgument:
      return "invalid-argument";
    case ble_handoff::Status::InvalidSize:
      return "invalid-size";
    case ble_handoff::Status::InvalidMagic:
      return "invalid-magic";
    case ble_handoff::Status::InvalidVersion:
      return "invalid-version";
    case ble_handoff::Status::InvalidLength:
      return "invalid-length";
    case ble_handoff::Status::InvalidCrc:
      return "invalid-crc";
    case ble_handoff::Status::SequenceOverflow:
      return "sequence-overflow";
    case ble_handoff::Status::NotFound:
      return "not-found";
    case ble_handoff::Status::OpenFailed:
      return "open-failed";
    case ble_handoff::Status::ReadFailed:
      return "read-failed";
    case ble_handoff::Status::WriteFailed:
      return "write-failed";
    case ble_handoff::Status::CommitFailed:
      return "commit-failed";
    case ble_handoff::Status::VerifyFailed:
      return "verify-failed";
  }
  return "unknown";
}

void logHeap(const char* phase) {
  Serial.printf("BLE-RX %s free=%u maxAlloc=%u\n", phase, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onDisconnect(BLEServer*) override { BLEDevice::startAdvertising(); }
};

class WriteCallbacks final : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    if (length == 0 || length > pendingPayload.size() || data == nullptr) {
      portENTER_CRITICAL(&pendingMux);
      rejectedLength = length;
      rejectionPending = true;
      portEXIT_CRITICAL(&pendingMux);
      return;
    }

    portENTER_CRITICAL(&pendingMux);
    std::memcpy(pendingPayload.data(), data, length);
    pendingLength = length;
    payloadPending = true;
    portEXIT_CRITICAL(&pendingMux);
  }
};

ServerCallbacks serverCallbacks;
WriteCallbacks writeCallbacks;

}  // namespace

void setup() {
  delay(250);
  Serial.begin(115200);
  logHeap("before-ble");

  if (!BLEDevice::init(DEVICE_NAME)) {
    Serial.println("BLE-RX init failed");
    return;
  }

  BLEServer* server = BLEDevice::createServer();
  if (server == nullptr) {
    Serial.println("BLE-RX server allocation failed");
    return;
  }
  server->setCallbacks(&serverCallbacks);

  BLEService* service = server->createService(SERVICE_UUID);
  BLECharacteristic* writable = service->createCharacteristic(WRITE_UUID, BLECharacteristic::PROPERTY_WRITE);
  writable->setCallbacks(&writeCallbacks);
  service->start();

  BLEAdvertising* advertising = server->getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(false);
  advertising->start();
  logHeap("advertising");
  Serial.printf("BLE-RX advertising name=%s\n", DEVICE_NAME);
}

void loop() {
  std::array<uint8_t, ble_handoff::MAX_PAYLOAD_SIZE> payload{};
  size_t length = 0;
  size_t rejected = 0;
  bool hadRejection = false;

  portENTER_CRITICAL(&pendingMux);
  if (payloadPending) {
    length = pendingLength;
    std::memcpy(payload.data(), pendingPayload.data(), length);
    payloadPending = false;
  }
  if (rejectionPending) {
    rejected = rejectedLength;
    rejectionPending = false;
    hadRejection = true;
  }
  portEXIT_CRITICAL(&pendingMux);

  if (hadRejection) {
    Serial.printf("BLE-RX rejected length=%u\n", static_cast<unsigned>(rejected));
  }

  if (length == 0) {
    delay(10);
    return;
  }

  ble_handoff::DecodedRecord persisted{};
  const ble_handoff::Status status = ble_handoff::persistAndVerify(payload.data(), length, persisted);
  if (status != ble_handoff::Status::Ok) {
    Serial.printf("BLE-RX persist failed status=%s\n", statusName(status));
    return;
  }

  Serial.printf("PERSISTED sequence=%u length=%u crc=%08x free=%u maxAlloc=%u payload=", persisted.sequence,
                static_cast<unsigned>(persisted.length), persisted.crc, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  Serial.write(persisted.payload.data(), persisted.length);
  Serial.println();
  delay(100);
  ESP.restart();
}
