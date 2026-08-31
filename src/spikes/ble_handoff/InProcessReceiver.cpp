#if defined(CROSSINK_BLE_HANDOFF_RECEIVER) || defined(CROSSINK_IN_PROCESS_RECEIVER)

#include "InProcessReceiver.h"

#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <esp_bt.h>
#include <freertos/FreeRTOS.h>

#include <array>
#include <cstring>

#include "BleHandoffNvs.h"
#include "DashboardSlotSelection.h"
#include "DashboardTransfer.h"
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

// Written from the NimBLE host task and read from the reader task, hence
// volatile: `teardownReceiver` waits on them to avoid pulling the stack out
// from under an event that is still in flight.
volatile bool clientConnected = false;
volatile bool tearingDown = false;

// Kept so teardown can close the link itself. BLEDevice has no accessor for the
// server it owns, and by the time teardown needs it the window is long out of
// scope.
BLEServer* activeServer = nullptr;

class ServerCallbacks final : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    clientConnected = true;
    Serial.println("BLE-RX connected");
    notify(0x01, 0, 0);
  }
  void onDisconnect(BLEServer*) override {
    clientConnected = false;
    Serial.println("BLE-RX disconnected");
    // Re-advertising is what keeps the window usable after a client drops
    // mid-transfer, but during teardown it rebuilds exactly what is being
    // dismantled — the partition receiver never had to care, because it
    // restarted instead of shutting the stack down.
    if (!tearingDown) BLEDevice::startAdvertising();
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
  activeServer = server;
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
  tearingDown = true;

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) advertising->stop();

  // The link has to be down before deinit, and this side has to be the one that
  // closes it. BLEDevice::deinit() deletes the BLEServer and only then calls
  // nimble_port_stop(), so a link that is still up gets terminated by the stop
  // and its disconnect event is handed to the deleted server -- removePeerDevice
  // erases from a freed std::map and the heap poison check aborts. Confirmed on
  // hardware 2026-08-31 by forcing it: deinit with a live link fails every time.
  //
  // Waiting for the phone to disconnect on its own is what this used to do, and
  // it is not ours to schedule: a backgrounded iOS app can take seconds, which
  // is how the panic of that morning happened. Terminating from here is a local
  // HCI command that the controller confirms in milliseconds.
  const bool closedFromHere = activeServer != nullptr && clientConnected;
  if (closedFromHere) {
    const int status = activeServer->disconnect(activeServer->getConnId());
    if (status != 0) Serial.printf("BLE-RX teardown: disconnect returned %d\n", status);
  }

  // Bounded anyway: a phone that vanishes mid-window must not hold the boot
  // hostage, and a link the controller cannot terminate is a case we handle
  // below rather than wait out.
  constexpr uint32_t DISCONNECT_CONFIRM_MS = 1000;
  const uint32_t waitStart = millis();
  const uint32_t waitUntil = waitStart + DISCONNECT_CONFIRM_MS;
  while (clientConnected && millis() < waitUntil) delay(10);
  const uint32_t confirmMs = millis() - waitStart;

  // Even with the link down the host task may still be draining events. This
  // is the difference between "no connection" and "nothing in flight".
  delay(150);

  // Unconditional, because "no crash" on its own cannot be told apart from "the
  // phone happened to disconnect first this time" -- and that ambiguity is
  // exactly what let the panic sit unexplained. closedFromHere=0 over many
  // windows would mean this teardown is not doing the work its comment claims.
  Serial.printf("BLE-RX teardown: closedFromHere=%d confirmMs=%lu stillUp=%d\n", closedFromHere ? 1 : 0,
                static_cast<unsigned long>(confirmMs), clientConnected ? 1 : 0);

  if (clientConnected) {
    // Deinit here would be the use-after-free above, deliberately entered. The
    // stack stays up instead, which costs the reader the ~27 KB it would have
    // got back for the rest of this boot -- the next deep sleep clears it
    // either way. A short heap degrades a chapter layout; a panic loses the
    // whole card and leaves a crash screen on the panel.
    Serial.println("BLE-RX teardown: link still up, skipping deinit");
    return;
  }

  BLEDevice::deinit(true);
  activeServer = nullptr;
}

void releaseBluetoothMemory(const char* reason) {
  static bool released = false;
  if (released) return;
  released = true;

  const uint32_t before = ESP.getHeapSize();
  // ESP_BT_MODE_BLE rather than BTDM: this is a BLE-only controller, and the
  // header names exactly this call as the way to hand the stack's BSS and data
  // back when Bluetooth is not needed again this boot. BLEDevice::deinit(true)
  // already releases the *controller* memory, but only on the path that ran the
  // window, and it never touches the host BSS.
  const esp_err_t status = esp_bt_mem_release(ESP_BT_MODE_BLE);
  const uint32_t after = ESP.getHeapSize();
  Serial.printf("BLE-MEM release (%s) status=%d heapSize=%u->%u delta=%d\n", reason, static_cast<int>(status),
                static_cast<unsigned>(before), static_cast<unsigned>(after),
                static_cast<int>(after) - static_cast<int>(before));
}

}  // namespace dashboard

#endif
